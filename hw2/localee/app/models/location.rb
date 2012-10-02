class Location < ActiveRecord::Base
  # attr_accessible :title, :body

  has_and_belongs_to_many :users
  has_many :posts

  def to_hash
    {
      :id => self.id,
      :name => self.name,
      :latitude => self.latitude,
      :longitude => self.longitude
    }
  end
end
