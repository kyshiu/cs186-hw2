class Location < ActiveRecord::Base
  # attr_accessible :title, :body

  has_and_belongs_to_many :users
  has_many :posts
  
  # Validations
  validates :name, :presence => true
  validates :latitude, :presence => true
  validates :longitude, :presence => true

  def to_hash
    {
      :id => self.id,
      :name => self.name,
      :latitude => self.latitude,
      :longitude => self.longitude
    }
  end
  
  def to_hash_with_follow(user_id)
    {
      :id => self.id,
      :name => self.name,
      :latitude => self.latitude,
      :longitude => self.longitude,
      :follows => User.find(user_id).locations.exists?("id == ?", self.id)
    }
  end
end
